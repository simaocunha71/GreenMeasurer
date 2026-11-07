/* Overtime runs the sampler in the parent process, without sampling threads. */
#ifndef OT_MAX_SAMPLES
#define OT_MAX_SAMPLES 262144
#endif
#define OT_MAX_METRICS 64
#define OT_SCALAR_COUNT 4

struct OvertimeContext {
    const char *language;
    const char *program;
    int powercap;
    const char *output_file;
};

static double ot_scalar_values[OT_SCALAR_COUNT][OT_MAX_SAMPLES];
static float ot_freq_mhz[OT_MAX_SAMPLES][SYS_MAX_CORES];
static double ot_backend_values[OT_MAX_METRICS][OT_MAX_SAMPLES];
static double ot_timestamp_ms[OT_MAX_SAMPLES];
static size_t ot_count;
static int ot_core_count;
static int ot_backend_ready[2];
static int (*ot_sample_next[2])(int);
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
    for (size_t i = 0; i < loaded_backend_count; i++) {
        const EnergyBackendDescriptor *descriptor = loaded_backends[i].descriptor;
        if (!descriptor) continue;
        EnergyBackendType type = descriptor->type;
        if ((type == ENERGY_BACKEND_CPU && descriptor->backend.cpu == active_cpu_backend) ||
            (type == ENERGY_BACKEND_GPU && descriptor->backend.gpu == active_gpu_backend)) {
            ot_sample_next[type] = dlsym(loaded_backends[i].handle,
                                         ENERGY_BACKEND_SAMPLE_NEXT_SYMBOL);
        }
    }
}

static int ot_prepare(void) {
    if (active_metric_count > OT_MAX_METRICS) {
        fprintf(stderr, "[ERROR] Too many metrics for overtime sampling (max %d).\n", OT_MAX_METRICS);
        return -1;
    }
    ot_count = 0;
    ot_core_count = sys_core_count();
    if (ot_core_count > SYS_MAX_CORES) ot_core_count = SYS_MAX_CORES;
    ot_find_next_callbacks();
    for (int type = 0; type < 2; type++) {
        int present = type == ENERGY_BACKEND_CPU ? active_cpu_backend != NULL : active_gpu_backend != NULL;
        if (!present) continue;
        int initialized = type == ENERGY_BACKEND_CPU
            ? active_cpu_backend->init(0) == 0 : active_gpu_backend->init(0) == 0;
        ot_backend_ready[type] = initialized && ot_backend_start((EnergyBackendType)type) == 0;
        if (!ot_backend_ready[type]) {
            fprintf(stderr, "[WARNING] Failed to start overtime %s backend window; "
                    "window metrics are unavailable.\n", type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
        }
        if (ot_backend_ready[type] && !ot_sample_next[type]) {
            fprintf(stderr, "[WARNING] %s backend does not support contiguous overtime windows; "
                    "energy between windows will not be recorded.\n", type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
        }
    }
    return 0;
}

static int ot_sample(pid_t workload_pid, int finished) {
    if (ot_count >= OT_MAX_SAMPLES) {
        fprintf(stderr, "[ERROR] Overtime sample capacity (%d) exceeded; measurement aborted.\n", OT_MAX_SAMPLES);
        return -1;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("[ERROR] Failed to timestamp overtime sample");
        return -1;
    }
    size_t slot = ot_count++;
    ot_timestamp_ms[slot] = (double)(now.tv_sec - ot_start_time.tv_sec) * 1000.0 +
                            (double)(now.tv_nsec - ot_start_time.tv_nsec) / 1000000.0;
    int stopped[2] = {0, 0};
    for (int type = 0; type < 2; type++) {
        if (!ot_backend_ready[type]) continue;
        stopped[type] = (ot_sample_next[type] ? ot_sample_next[type](0)
                                              : ot_backend_stop((EnergyBackendType)type)) == 0;
    }

    ot_scalar_values[0][slot] = sys_workload_memory_kb(workload_pid);
    ot_scalar_values[1][slot] = (double)sys_core_count();
    ot_scalar_values[2][slot] = sys_workload_process_count(workload_pid);
    ot_scalar_values[3][slot] = sys_running_processes();
    for (int core = 0; core < ot_core_count; core++) {
        ot_freq_mhz[slot][core] = (float)sys_core_frequency_mhz(core);
    }
    for (size_t i = 0; i < active_metric_count; i++) {
        const ActiveMetric *metric = &active_metrics[i];
        int type = metric->backend_type;
        ot_backend_values[i][slot] =
            metric->descriptor->kind == ENERGY_METRIC_WINDOW && !stopped[type]
                ? METRIC_NOT_SUPPORTED
                : metric->descriptor->scope == ENERGY_METRIC_PROCESS
                    ? read_process_tree_metric(i, workload_pid) : read_active_metric(i, 0);
    }
    for (int type = 0; type < 2; type++) {
        if (!ot_backend_ready[type]) continue;
        if (!stopped[type]) {
            fprintf(stderr, "[WARNING] Overtime %s backend sample failed; window metrics are unavailable.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            ot_backend_ready[type] = 0;
        } else if (!finished && !ot_sample_next[type] && ot_backend_start((EnergyBackendType)type) != 0) {
            fprintf(stderr, "[WARNING] Failed to restart overtime %s backend window.\n",
                    type == ENERGY_BACKEND_CPU ? "CPU" : "GPU");
            ot_backend_ready[type] = 0;
        }
    }
    return 0;
}

static int ot_dump(const struct OvertimeContext *ctx) {
    FILE *fp = fopen(ctx->output_file, "w");
    if (!fp) {
        perror("[ERROR] Failed to open overtime CSV");
        return -1;
    }
    energy_backend_write_csv_header(fp, 1);
    for (size_t row = 0; row < ot_count; row++) {
        fprintf(fp, "%s, %s, %d, %.6f", ctx->language, ctx->program, ctx->powercap,
                ot_timestamp_ms[row]);
        for (int i = 0; i < OT_SCALAR_COUNT; i++) {
            fprintf(fp, ", ");
            write_active_metric_value(fp, ot_scalar_values[i][row]);
        }
        for (int core = 0; core < ot_core_count; core++) {
            fprintf(fp, ", ");
            write_active_metric_value(fp, (double)ot_freq_mhz[row][core]);
        }
        for (size_t i = 0; i < active_metric_count; i++) {
            fprintf(fp, ", ");
            write_active_metric_value(fp, ot_backend_values[i][row]);
        }
        fputc('\n', fp);
    }
    int failed = ferror(fp);
    if (fclose(fp) != 0) failed = 1;
    if (failed) fprintf(stderr, "[ERROR] Failed to write overtime CSV.\n");
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
        fprintf(stderr, "[ERROR] Failed to initialize overtime sampling.\n");
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
                if (ot_dump(ctx) != 0) result = -1;
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
            fprintf(stderr, "[ERROR] Overtime measurement interrupted.\n");
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

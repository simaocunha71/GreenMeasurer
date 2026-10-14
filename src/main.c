#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <locale.h>
#include <errno.h>

#include "include/energy-backend.h"
#include "include/sensors.h"

/* Active backend references */
static EnergyBackend *active_cpu_backend = NULL;
static EnergyBackend *active_gpu_backend = NULL;

/* Configurable paths */
static char calib_file[512] = "/tmp/cores_temperatures.txt";
static char measurements_file[512] = "measurements.csv";

static volatile sig_atomic_t global_terminate = 0;
static void sigint_handler(int signum) { 
    (void)signum; 
    global_terminate = 1; 
}

static struct { int returnValue; } threadData;

/* Shared metric formatters */
static void write_energy_metric(FILE *fp, double val) {
    if (val < 0.0) {
        fprintf(fp, "NOT_SUPPORTED");
    } else {
        fprintf(fp, "%.18f", val);
    }
}

static void write_temperature_metric(FILE *fp, float val) {
    if (val < -0.5f) {
        fprintf(fp, "NOT_SUPPORTED");
    } else {
        fprintf(fp, "%.1f", val);
    }
}

static void writeErrorMessage(const char *language, const char *program, const char *errorMsg, 
                              const char *out_file, int powercap) {
    FILE *fp = fopen(out_file, "a");
    if (!fp) return;
    fprintf(fp, "%s, %s, %d, %s, %s, %s, %s, %s, %s, %s, %s\n",
            language, program, powercap,
            errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg, errorMsg);
    fclose(fp);
}

static double calibrateTemperature(const char *target_calib_file, int sleep_seconds) {
    printf("[INFO] Sleeping %d seconds to stabilize and calibrate CPU temperature...\n", sleep_seconds);
    if (sleep_seconds > 0) {
        sleep(sleep_seconds);
    }

    if (dump_temperature_baseline(target_calib_file) != 0) {
        fprintf(stderr, "[WARNING] Failed to generate baseline temperature dump in %s\n", target_calib_file);
    }

    float mean_temp = getTemperature(0);
    if (mean_temp <= 0.0f) {
        fprintf(stderr, "[WARNING] Sensor read returned <= 0.0, defaulting baseline to 40.0°C\n");
        mean_temp = 40.0f;
    }

    FILE *output_file = fopen(target_calib_file, "a");
    if (output_file != NULL) {
        fprintf(output_file, "Average temperature: %.1f°C\n\n", mean_temp);
        fclose(output_file);
        printf("[INFO] Calibrated baseline temperature saved to %s: %.1f°C\n", target_calib_file, mean_temp);
    } else {
        perror("[ERROR] Failed to write calibration file");
    }

    return (double)mean_temp;
}

static void init_backends(void) {
    EnergyBackend *candidates[] = {
        &intel_backend,
        &amd_backend,
        &mac_backend,
        NULL
    };

    for (int i = 0; candidates[i] != NULL; i++) {
        if (candidates[i] && candidates[i]->detect && candidates[i]->detect() == 1) {
            active_cpu_backend = candidates[i];
            printf("[INFO] Selected CPU Energy Backend: %s\n", active_cpu_backend->name);
            break;
        }
    }

    if (!active_cpu_backend) {
        fprintf(stderr, "[ERROR] No compatible CPU energy backend detected for this machine.\n");
        exit(EXIT_FAILURE);
    }

    if (&nvidia_backend && nvidia_backend.detect && nvidia_backend.detect() == 1) {
        active_gpu_backend = &nvidia_backend;
        printf("[INFO] Selected GPU Energy Backend: %s\n", active_gpu_backend->name);
    }
}

static void runCommand(const char *cmd) {
    if (system(cmd) == -1) exit(EXIT_FAILURE);
}

static void* programWorks(void *arg) {
    FILE *fp = popen((const char *)arg, "r");
    if (!fp) { 
        threadData.returnValue = 0; 
        pthread_exit(NULL); 
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {}
    int status = pclose(fp);
    threadData.returnValue = (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
    pthread_exit(NULL);
}

struct MeasureArgs {
    FILE *fp; 
    int core; 
    const char *command; 
    int memory; 
    float temp;
};

static void* measureWorker(void *arg) {
    struct MeasureArgs *args = (struct MeasureArgs *)arg;
    struct timeval tvb, tva;

    EnergyMetrics cpu_m = {-1.0, -1.0, -1.0, -1.0};
    EnergyMetrics gpu_m = {-1.0, -1.0, -1.0, -1.0};

    if (active_cpu_backend && active_cpu_backend->init) active_cpu_backend->init(args->core);
    if (active_gpu_backend && active_gpu_backend->init) active_gpu_backend->init(0);

    if (active_cpu_backend && active_cpu_backend->before) active_cpu_backend->before(args->core);
    if (active_gpu_backend && active_gpu_backend->before) active_gpu_backend->before(0);

    gettimeofday(&tvb, NULL);
    runCommand(args->command);
    gettimeofday(&tva, NULL);

    if (active_cpu_backend && active_cpu_backend->after) active_cpu_backend->after(args->core, &cpu_m);
    if (active_gpu_backend && active_gpu_backend->after) active_gpu_backend->after(0, &gpu_m);

    double elapsed_ms = ((tva.tv_sec - tvb.tv_sec) * 1000000.0 + (tva.tv_usec - tvb.tv_usec)) / 1000.0;
    double final_gpu_energy = (active_gpu_backend != NULL) ? gpu_m.gpu : cpu_m.gpu;
    float gpu_temp = getGpuTemperature();

    write_energy_metric(args->fp, cpu_m.package);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, cpu_m.core);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, final_gpu_energy);
    fprintf(args->fp, ", ");
    write_energy_metric(args->fp, cpu_m.dram);
    fprintf(args->fp, ", ");
    fprintf(args->fp, "%G, ", elapsed_ms);
    write_temperature_metric(args->fp, args->temp);
    fprintf(args->fp, ", ");
    write_temperature_metric(args->fp, gpu_temp);
    fprintf(args->fp, ", ");

    if (args->memory < 0) {
        fprintf(args->fp, "NOT_SUPPORTED\n");
    } else {
        fprintf(args->fp, "%d\n", args->memory);
    }

    fflush(args->fp);
    free(args);
    return NULL;
}

void runTesting(int ntimes, int core, const char *command, const char *language, 
                const char *program, const char *output_file, int powercap, 
                int time_out_limit, float temp_threshold, int variance, int memory) {
    struct timespec ts;
    pthread_t th;

    /* 1. Verify program functionality and enforce execution timeout during pre-check */
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        perror("[ERROR] clock_gettime");
        return;
    }
    ts.tv_sec += time_out_limit;

    threadData.returnValue = 0;
    if (pthread_create(&th, NULL, programWorks, (void *)command) != 0) {
        writeErrorMessage(language, program, "ThreadCreateError", output_file, powercap);
        return;
    }

    int state = pthread_timedjoin_np(th, NULL, &ts);
    if (state != 0) {
        if (state == ETIMEDOUT) {
            printf("[WARNING] Timeout: Program pre-check exceeded %d seconds\n", time_out_limit);
            writeErrorMessage(language, program, "TimeOut", output_file, powercap);
        } else {
            writeErrorMessage(language, program, "JoinError", output_file, powercap);
        }
        return;
    }

    if (threadData.returnValue != 1) {
        writeErrorMessage(language, program, "ProgramError", output_file, powercap);
        return;
    }

    FILE *fp = fopen(output_file, "a");
    if (!fp) return;

    /* 2. Execute measurement repetitions with thermal control and timeout handling */
    for (int i = 0; i < ntimes; i++) {
        float temp = getTemperature(0);
        while (temp > temp_threshold + variance) {
            printf("[INFO] Temperature %.1f°C > Baseline %.1f°C + %d°C. Waiting 1s...\n", 
                   temp, temp_threshold, variance);
            sleep(1);
            temp = getTemperature(0);
        }

        fprintf(fp, "%s, %s, %d, ", language, program, powercap);

        if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
            perror("[ERROR] clock_gettime");
            fclose(fp);
            return;
        }
        ts.tv_sec += time_out_limit;

        struct MeasureArgs *m = malloc(sizeof(struct MeasureArgs));
        m->fp = fp; 
        m->core = core; 
        m->command = command; 
        m->memory = memory; 
        m->temp = temp;

        if (pthread_create(&th, NULL, measureWorker, m) != 0) {
            free(m);
            writeErrorMessage(language, program, "ThreadCreateError", output_file, powercap);
            fclose(fp);
            return;
        }

        state = pthread_timedjoin_np(th, NULL, &ts);
        if (state != 0) {
            if (state == ETIMEDOUT) {
                printf("[WARNING] Timeout: Execution exceeded %d seconds\n", time_out_limit);
                writeErrorMessage(language, program, "TimeOut", output_file, powercap);
            } else {
                writeErrorMessage(language, program, "JoinError", output_file, powercap);
            }
            fclose(fp);
            return;
        }
    }
    fclose(fp);
}

/* Real-time background sampling context for overtime mode */
struct OvertimeThreadCtx {
    volatile int run_flag;
    int interval_ms;
    const char *language;
    const char *program;
    int powercap;
    const char *output_file;
};

static void* overtimeLogger(void *arg) {
    struct OvertimeThreadCtx *ctx = (struct OvertimeThreadCtx *)arg;
    FILE *fp = fopen(ctx->output_file, "w");
    if (!fp) {
        perror("[ERROR] Failed to open overtime CSV");
        pthread_exit(NULL);
    }

    fprintf(fp, "Language,Program,Powercap,Package,Core,GPU,DRAM,Timestamp,CpuTemperature,GpuTemperature\n");
    fflush(fp);

    if (active_cpu_backend && active_cpu_backend->init) active_cpu_backend->init(0);
    if (active_gpu_backend && active_gpu_backend->init) active_gpu_backend->init(0);

    /* 1. Amostra inicial de referência (T=0) */
    if (active_cpu_backend && active_cpu_backend->before) active_cpu_backend->before(0);
    if (active_gpu_backend && active_gpu_backend->before) active_gpu_backend->before(0);

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    long interval_ns = (long)ctx->interval_ms * 1000000L;
    unsigned long current_timestamp = 0;

    while (ctx->run_flag && !global_terminate) {
        /* Avança para o próximo tick absoluto do relógio */
        next_tick.tv_nsec += interval_ns;
        while (next_tick.tv_nsec >= 1000000000L) {
            next_tick.tv_nsec -= 1000000000L;
            next_tick.tv_sec += 1;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        if (!ctx->run_flag || global_terminate) break;

        current_timestamp += ctx->interval_ms;

        EnergyMetrics cpu_m = {-1.0, -1.0, -1.0, -1.0};
        EnergyMetrics gpu_m = {-1.0, -1.0, -1.0, -1.0};

        /* Lê as métricas atuais consumidas desde o início */
        if (active_cpu_backend && active_cpu_backend->after) active_cpu_backend->after(0, &cpu_m);
        if (active_gpu_backend && active_gpu_backend->after) active_gpu_backend->after(0, &gpu_m);

        double final_gpu_energy = (active_gpu_backend != NULL) ? gpu_m.gpu : cpu_m.gpu;
        float current_cpu_temp = getTemperature(0);
        float current_gpu_temp = getGpuTemperature();

        /* Registo na linha do CSV */
        fprintf(fp, "%s, %s, %d, ", ctx->language, ctx->program, ctx->powercap);
        write_energy_metric(fp, cpu_m.package);
        fprintf(fp, ", ");
        write_energy_metric(fp, cpu_m.core);
        fprintf(fp, ", ");
        write_energy_metric(fp, final_gpu_energy);
        fprintf(fp, ", ");
        write_energy_metric(fp, cpu_m.dram);
        fprintf(fp, ", %lu, ", current_timestamp);
        write_temperature_metric(fp, current_cpu_temp);
        fprintf(fp, ", ");
        write_temperature_metric(fp, current_gpu_temp);
        fprintf(fp, "\n");
        fflush(fp);

        /* NÃO CHAMAR active_cpu_backend->before(0) AQUI! 
           Desta forma cada amostragem reflete o valor acumulado real 
           desde o início da execução até este Timestamp */
    }

    fclose(fp);
    pthread_exit(NULL);
    return NULL;
}

static void show_help(const char *bin_name) {
    printf("\nUsage:\n");
    printf("  %s --temperature-calibration [-sleep-secs <sec>] [-calib-file <path>]\n", bin_name);
    printf("      Calibrates baseline temperature and stores it in the calibration file.\n\n");

    printf("  %s --powercap-calibration -range <min>-<max> -variance <V> -time_out_limit <hours> -n_times <N> -secs_to_sleep <sec> [-compile_cmd \"<cmd>\"] -command \"<cmd>\" -language <lang> -program <prog> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Calibrates powercap limits and evaluates consumption curves.\n\n");

    printf("  %s --standard -command \"<cmd>\" -language <lang> -program <prog> -n_times <N> -variance <V> -time_out_limit <hours> -sleep_time <sec> -powercap <W> [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Runs standard aggregate energy measurements.\n\n");

    printf("  %s --overtime -command \"<cmd>\" -interval <ms> -language <lang> -program <prog> [-powercap <W>] [-variance <V>] [-sleep-secs <sec>] [-calib-file <path>] [-output <path>]\n", bin_name);
    printf("      Continuously measures energy over time at fixed sampling intervals.\n\n");
}

/* Resolves Python executable prioritizing the repository's virtual environment */
static const char* resolve_python_venv(void) {
    if (access(".venv/bin/python3", X_OK) == 0) return ".venv/bin/python3";
    if (access("../.venv/bin/python3", X_OK) == 0) return "../.venv/bin/python3";
    if (access("venv/bin/python3", X_OK) == 0) return "venv/bin/python3";
    if (access("../venv/bin/python3", X_OK) == 0) return "../venv/bin/python3";
    return "python3";
}

int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "C");
    signal(SIGINT, sigint_handler);

    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        show_help(argv[0]);
        return 0;
    }

    init_backends();

    /* ========================================================
       1. MODE: TEMPERATURE CALIBRATION STANDALONE
       ======================================================== */
    if (argc >= 2 && strcmp(argv[1], "--temperature-calibration") == 0) {
        int sleep_secs = 20;
        for (int i = 2; i < argc; ++i) {
            if ((!strcmp(argv[i], "-sleep-secs") || !strcmp(argv[i], "-sleep_time")) && i + 1 < argc) {
                sleep_secs = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
        }
        calibrateTemperature(calib_file, sleep_secs);
        return 0;
    }

    /* ========================================================
       2. MODE: POWERCAP CALIBRATION
       ======================================================== */
    if (argc >= 2 && strcmp(argv[1], "--powercap-calibration") == 0) {
        if (!active_cpu_backend || !active_cpu_backend->calibrate_powercap) {
            fprintf(stderr, "[ERROR] Powercap calibration is not supported by backend: %s\n",
                    active_cpu_backend ? active_cpu_backend->name : "None");
            return 1;
        }

        int min_powercap = -1, max_powercap = -1;
        int variance = 5, timeout_hours = 1, sleep_secs = 10, ntimes = 3;
        char command[500] = "", compile_cmd[500] = "", language[128] = "", program[128] = "";
        char calib_csv[512] = "powercap_calibration.csv";

        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "-range") && i + 1 < argc) {
                sscanf(argv[++i], "%d-%d", &min_powercap, &max_powercap);
            }
            else if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
                strncpy(command, argv[++i], sizeof(command) - 1);
            }
            else if (!strcmp(argv[i], "-compile_cmd") && i + 1 < argc) {
                strncpy(compile_cmd, argv[++i], sizeof(compile_cmd) - 1);
            }
            else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
            else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
            else if (!strcmp(argv[i], "-n_times") && i + 1 < argc) ntimes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-time_out_limit") && i + 1 < argc) timeout_hours = atoi(argv[++i]);
            else if ((!strcmp(argv[i], "-secs_to_sleep") || !strcmp(argv[i], "-sleep_time")) && i + 1 < argc) {
                sleep_secs = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
            else if (!strcmp(argv[i], "-output") && i + 1 < argc) {
                strncpy(calib_csv, argv[++i], sizeof(calib_csv) - 1);
            }
        }

        if (min_powercap < 0 || max_powercap < 0 || strlen(command) == 0) {
            fprintf(stderr, "[ERROR] --powercap-calibration: missing -range or -command\n");
            return 1;
        }

        if (strlen(compile_cmd) > 0) {
            printf("[INFO] Compiling benchmark target: %s\n", compile_cmd);
            if (system(compile_cmd) != 0) {
                fprintf(stderr, "[ERROR] Compilation command failed.\n");
                return 1;
            }
        }

        int memory = 0;
        if (active_cpu_backend->measure_memory) {
            memory = active_cpu_backend->measure_memory(command);
        }

        if (access(calib_file, F_OK) != 0) {
            calibrateTemperature(calib_file, sleep_secs);
        }
        float ref_temp = getInitialTemperature(calib_file);
        if (ref_temp <= 0.0f) ref_temp = getTemperature(0);

        int timeout_seconds = timeout_hours * 3600;

        /* Execute modular calibration via the active backend */
        active_cpu_backend->calibrate_powercap(
            ntimes, 0, command, language, program, calib_csv,
            NULL, min_powercap, max_powercap, timeout_seconds,
            ref_temp, variance, memory
        );

        /* Automatic post-processing of the calibration CSV in the virtual environment */
        const char *py_bin = resolve_python_venv();
        char proc_cmd[1024];
        if (access("Utils/processPowercapCalibrationCSV.py", F_OK) == 0) {
            snprintf(proc_cmd, sizeof(proc_cmd), "%s Utils/processPowercapCalibrationCSV.py %s", py_bin, calib_csv);
        } else {
            snprintf(proc_cmd, sizeof(proc_cmd), "%s ../Utils/processPowercapCalibrationCSV.py %s", py_bin, calib_csv);
        }
        printf("[INFO] Processing calibration CSV via: %s\n", proc_cmd);
        if (system(proc_cmd) != 0) {
            fprintf(stderr, "[WARNING] Calibration post-processing script exited with error.\n");
        }

        return 0;
    }

    /* ========================================================
       3. MODE: OVERTIME
       ======================================================== */
    if (argc >= 2 && (!strcmp(argv[1], "--overtime") || !strcmp(argv[1], "--over-time"))) {
        char command[500] = "", language[128] = "", program[128] = "";
        char ot_csv[512] = "measurements_ot.csv";
        int interval_ms = 100, powercap = -1, variance = 5, sleep_time = 5;

        for (int i = 2; i < argc; ++i) {
            if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
                strncpy(command, argv[++i], sizeof(command) - 1);
            }
            else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
            else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
            else if (!strcmp(argv[i], "-interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-powercap") && i + 1 < argc) powercap = atoi(argv[++i]);
            else if ((!strcmp(argv[i], "-sleep_time") || !strcmp(argv[i], "-sleep-secs")) && i + 1 < argc) {
                sleep_time = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
                strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
            }
            else if (!strcmp(argv[i], "-output") && i + 1 < argc) {
                strncpy(ot_csv, argv[++i], sizeof(ot_csv) - 1);
            }
        }

        if (access(calib_file, F_OK) != 0) {
            calibrateTemperature(calib_file, sleep_time);
        }
        float ref_temp = getInitialTemperature(calib_file);
        if (ref_temp <= 0.0f) ref_temp = getTemperature(0);

        float temp = getTemperature(0);
        while (temp > ref_temp + variance) {
            printf("[INFO] Waiting for temperature (%.1f°C) to reach baseline (%.1f°C)...\n", temp, ref_temp);
            sleep(1);
            temp = getTemperature(0);
        }

        if (powercap > 0 && active_cpu_backend->set_powercap) {
            active_cpu_backend->set_powercap(powercap);
        }

        struct OvertimeThreadCtx ctx = {
            .run_flag = 1,
            .interval_ms = interval_ms,
            .language = language,
            .program = program,
            .powercap = powercap,
            .output_file = ot_csv
        };

        pthread_t logger_th;
        if (pthread_create(&logger_th, NULL, overtimeLogger, &ctx) != 0) {
            perror("[ERROR] Failed to start overtime sampling thread");
            return 1;
        }

        /* Run targeted benchmark command in foreground */
        runCommand(command);

        /* Signal worker loop completion and wait for thread to join */
        ctx.run_flag = 0;
        pthread_join(logger_th, NULL);

        if (powercap > 0 && active_cpu_backend->destroy_powercap) {
            active_cpu_backend->destroy_powercap();
        }
        printf("[INFO] Overtime session finished -> %s\n", ot_csv);
        return 0;
    }

    /* ========================================================
       4. MODE: STANDARD MEASUREMENT
       ======================================================== */
    char command[500] = "", language[128] = "", program[128] = "";
    int ntimes = 1, powercap = -1, variance = 0, timeout = 3600;
    int sleep_time = 5;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--standard")) {
            continue;
        }
        else if ((!strcmp(argv[i], "-command") || !strcmp(argv[i], "-run_cmd")) && i + 1 < argc) {
            strncpy(command, argv[++i], sizeof(command) - 1);
        }
        else if (!strcmp(argv[i], "-language") && i + 1 < argc) strncpy(language, argv[++i], sizeof(language) - 1);
        else if (!strcmp(argv[i], "-program") && i + 1 < argc) strncpy(program, argv[++i], sizeof(program) - 1);
        else if (!strcmp(argv[i], "-n_times") && i + 1 < argc) ntimes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-variance") && i + 1 < argc) variance = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-time_out_limit") && i + 1 < argc) timeout = atoi(argv[++i]) * 3600;
        else if (!strcmp(argv[i], "-powercap") && i + 1 < argc) powercap = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-sleep_time") || !strcmp(argv[i], "-sleep-secs")) && i + 1 < argc) {
            sleep_time = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-calib-file") && i + 1 < argc) {
            strncpy(calib_file, argv[++i], sizeof(calib_file) - 1);
        }
        else if ((!strcmp(argv[i], "-output") || !strcmp(argv[i], "-measurements-file")) && i + 1 < argc) {
            strncpy(measurements_file, argv[++i], sizeof(measurements_file) - 1);
        }
    }

    int memory = 0;
    if (strlen(command) > 0 && active_cpu_backend->measure_memory) {
        memory = active_cpu_backend->measure_memory(command);
    }

    if (access(calib_file, F_OK) != 0) {
        printf("[WARNING] Calibration file %s not found. Running temperature calibration...\n", calib_file);
        calibrateTemperature(calib_file, sleep_time);
    } else {
        printf("[INFO] Calibration file %s found. Skipping temperature calibration.\n", calib_file);
    }

    float ref_temp = getInitialTemperature(calib_file);
    if (ref_temp <= 0.0f) {
        ref_temp = getTemperature(0);
    }

    printf("\n[INFO]\n======= Execution Details ========\n");
    printf(" Command               : %s\n", command);
    printf(" Language              : %s\n", language);
    printf(" Program               : %s\n", program);
    printf(" N_times               : %d\n", ntimes);
    printf(" Variance              : %d°C\n", variance);
    printf(" Timeout               : %d seconds\n", timeout);
    printf(" Sleep_time            : %d seconds\n", sleep_time);
    if (powercap == -1) printf(" PowerCap              : No power cap\n"); 
    else printf(" PowerCap              : %d watts\n", powercap);
    printf(" Calibration file      : %s\n", calib_file);
    printf(" Baseline temperature  : %.2f °C\n", ref_temp);
    printf(" Output CSV            : %s\n", measurements_file);
    printf("====================================\n\n");

    if (powercap > 0 && active_cpu_backend->set_powercap) {
        active_cpu_backend->set_powercap(powercap);
    }

    FILE *fp = fopen(measurements_file, "w");
    if (fp) {
        fprintf(fp, "Language,Program,Powercap,Package,Core,GPU,DRAM,Time,CpuTemperature,GpuTemperature,Memory\n");
        fclose(fp);
    }

    runTesting(ntimes, 0, command, language, program, measurements_file, powercap, timeout, ref_temp, variance, memory);

    if (powercap > 0 && active_cpu_backend->destroy_powercap) {
        active_cpu_backend->destroy_powercap();
    }

    return 0;
}
/*
 * GreenMeasurer - Energy Consumption Measurement Framework
 * 
 * A comprehensive tool for measuring energy consumption of programs
 * across multiple programming languages using RAPL technology.
 * 
 * Authors: Simão Cunha, João Saraiva
 * University of Minho
 */
/*
 * Authors: Simão Cunha, João Saraiva
 * University of Minho, Portugal
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <powercap/powercap.h>
#include <raplcap/raplcap.h>
#include <math.h>
#include "rapl.h"
#include "sensors.h"
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <regex.h>
#include <sys/time.h>
#include <unistd.h>

void performMeasurements(const char *command, const char *language, const char *program, int ntimes, int core, const char* MEASUREMENTS_FILE, int POWERCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle);


// Structure to hold function return value and timeout flag
struct ThreadData {
    int returnValue;
};

// Global structure to communicate between threads
struct ThreadData threadData;


int initializeRapl(raplcap *rc, int POWERCAP){
    printf("Running with CAP\n");
    raplcap_limit rl_short, rl_long;
    uint32_t q, j, n, d;

    // initialize
    if (raplcap_init(rc))
    {// Signal handler for SIGALRM
        perror("raplcap_init");
        return -1;
    }

    // get the number of RAPL packages
    n = raplcap_get_num_packages(NULL);
    if (n == 0)
    {
        perror("raplcap_get_num_packages");
        return -1;
    }

    // assuming each package has the same number of die, only querying for package=0
    d = raplcap_get_num_die(rc, 0);
    if (d == 0)
    {
        perror("raplcap_get_num_die");
        raplcap_destroy(rc);
        return -1;
    }

    // for each package die, set a power cap of same limit for both long and short
    // a time window of 0 leaves the time window unchanged
    rl_long.watts = POWERCAP;
    rl_long.seconds = 0;
    rl_short.watts = POWERCAP;
    rl_short.seconds = 0;

    for (q = 0; q < n; q++)
    {
        for (j = 0; j < d; j++)
        {
            if (raplcap_pd_set_limits(rc, q, j, RAPLCAP_ZONE_PACKAGE, &rl_long, &rl_short))
            {
                perror("raplcap_pd_set_limits");
            }
        }
    }

    // for each package die, enable the power caps
    // this could be done before setting caps, at the risk of enabling unknown power cap values first
    for (q = 0; q < n; q++)
    {
        for (j = 0; j < d; j++)
        {
            if (raplcap_pd_set_zone_enabled(rc, q, j, RAPLCAP_ZONE_PACKAGE, 1))
            {
                perror("raplcap_pd_set_zone_enabled");
            }
        }
    }
    return 0;
}

// Function to execute the command with a system call
void runCommand(const char *command) {
   int status = system(command);
   if (status == -1) {
       fprintf(stderr, "[ERROR] Error executing command\n");
       exit(EXIT_FAILURE);
   }
}

void* programWorks(void *arg) {
    const char *command = (const char *)arg;
    FILE *fp2 = popen(command, "r");
    if (fp2 == NULL) {
        fprintf(stderr, "[ERROR] Error running program verification\n");
        return 0;
    }
    
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp2) != NULL) {
    }
    int status = pclose(fp2);

    if (WIFEXITED(status)) {
        int exit_status = WEXITSTATUS(status);
        if (exit_status == 0) {
            printf("[INFO] Command executed successfully\n");
            threadData.returnValue = 1;
        } else {
            printf("[ERROR] Command exited with an error status: %d\n", exit_status);
            threadData.returnValue = 0;
        }
    } else {
        printf("[ERROR] Command did not exit normally\n");
        threadData.returnValue = 0;
    }
    pthread_exit(NULL);
};

void writeErrorMessage(const char *language, const char *program, const char *errorMsg, const char* MEASUREMENTS_FILE, int POWERCAP){
    FILE *fp;
    fp = fopen(MEASUREMENTS_FILE, "w");
    if (fp == NULL) {
        perror("[ERROR] Error opening measurements file");
        exit(-1);
    }

    fprintf(fp, "%s, %s, %d, %s, %s, %s, %s, %s, %s, %s\n", language, program, POWERCAP,
                        errorMsg,errorMsg,errorMsg,errorMsg,errorMsg,errorMsg,errorMsg);
    fclose(fp);
}

void runTesting(int ntimes, int core, const char *command, const char *language, const char *program, const char* MEASUREMENTS_FILE, 
    int POWERCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle) {    
   struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
        printf("[ERROR] Unexpected behaviour\n");
        return;
    }

    ts.tv_sec += TIME_OUT_LIMIT;

    // Create a thread to execute the function
    pthread_t thread;
    pthread_create(&thread, NULL, programWorks, (void *)command);
    
    // Join the thread to ensure its completion and get the return value
    int state = pthread_timedjoin_np(thread, NULL, &ts);

    if (state!=0) {
        // Handle timeout: Function execution interrupted
        writeErrorMessage(language,program,"TimeOut", MEASUREMENTS_FILE, POWERCAP);
        printf("[WARNING] Timeout: Function execution interrupted after %d seconds\n", TIME_OUT_LIMIT);
    } else {
        // Function executed within the specified timeout
        if(threadData.returnValue==1){ //Program works
            performMeasurements(command,language,program,ntimes,core, MEASUREMENTS_FILE, POWERCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, idle);
        }
        else{
            writeErrorMessage(language,program,"ProgramError", MEASUREMENTS_FILE, POWERCAP);
        }
    }
}

struct MeasureArgs {
    FILE *fp;
    int core;
    const char *command;
    int memory;
    float temperature; //This is the Average CPU temperature right before the execution
};

void* measure(void *arg) {
    struct MeasureArgs *args = (struct MeasureArgs *)arg;
    double time_spent;
    struct timeval tvb, tva;
    char str_temp[20];

    // Keep trying to initialize the RAPL interface for the specified core until success
    while (rapl_init(args->core) != 0);
    
    // Infinite loop to run the measurement and command execution process
    while (1) {
        // Attempt to read power/energy state before running the command
        // If it fails (returns -1), reinitialize RAPL and retry the loop
        if (rapl_before(args->fp, args->core) == -1) {
            while (rapl_init(args->core) != 0);
            continue;   // Retry the measurement and command execution
        }
    
        // Record the timestamp just before running the command
        gettimeofday(&tvb, NULL);
    
        // Execute the user-specified command
        runCommand(args->command);
    
        // Record the timestamp immediately after the command finishes
        gettimeofday(&tva, NULL);
    
        // Attempt to read power/energy state after running the command
        // If it fails, reinitialize RAPL and retry the loop
        if (rapl_after(args->fp, args->core) == -1) {
            while (rapl_init(args->core) != 0);
            continue;   // Retry the measurement and command execution
        }

    // If all steps succeeded, exit the loop
    break;
}

    
    
    time_spent = ((tva.tv_sec - tvb.tv_sec) * 1000000 + tva.tv_usec - tvb.tv_usec) / 1000;
    sprintf(str_temp, "%.1f", args->temperature);
    fprintf(args->fp, "%G, %s, %d\n", time_spent, str_temp, args->memory);
    fflush(args->fp);
}

// Function to perform the measurements and write to the measurements file
void performMeasurements(const char *command, const char *language, const char *program, int ntimes, int core, const char* MEASUREMENTS_FILE, 
    int POWERCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle) {
    struct timespec ts;

    FILE *fp;
    float temperature;

    if (idle == 0) {
       fp = fopen(MEASUREMENTS_FILE, "a");
       if (fp == NULL) {
           perror("[ERROR] Error opening measurements file for appending");
           exit(-1);
       }
    }
    else {
       fp = fopen(MEASUREMENTS_FILE, "w");
       if (fp == NULL) {
           perror("[ERROR] Error opening measurements file for writing");
           exit(-1);
       }

       fprintf(fp, "Language,Program,PowerLimit,Package,Core,GPU,DRAM,Time,Temperature,Memory\n");
    }


    for (int i = 0; i < ntimes; i++) {
       if (idle == 1) {
           fprintf(fp, "%s, idle_%s, %d, ", language, program, POWERCAP);
       }
       else {
           fprintf(fp, "%s, %s, %d, ", language, program, POWERCAP);
       }
       temperature = getTemperature();

       while (temperature > TEMPERATURETHRESHOLD + VARIANCE) {
           printf("[INFO] Sleeping 1 second... Current temperature: %.1f°C | Reference temperature: %.1f°C + Variance: %d°C | Waiting for temperature to drop below reference...\n", temperature, TEMPERATURETHRESHOLD, VARIANCE);
           sleep(1);
           temperature = getTemperature();
       }
       
       if (clock_gettime(CLOCK_REALTIME, &ts) == -1){
           printf("[ERROR] Unexpected behaviour\n");
           return;
       }

       ts.tv_sec += TIME_OUT_LIMIT;

       pthread_t thread;
       struct MeasureArgs* myArgs = (struct MeasureArgs*)malloc(sizeof(struct MeasureArgs));
       myArgs->command=command;
       myArgs->core=core;
       myArgs->fp=fp;
       myArgs->temperature=temperature;
       myArgs->memory=memory;
       pthread_create(&thread, NULL, measure, (void *)myArgs);
           
       // Join the thread to ensure its completion and get the return value
       int state = pthread_timedjoin_np(thread, NULL, &ts);
       if (state!=0) {
           // Handle timeout: Function execution interrupted
           writeErrorMessage(language,program,"TimeOut", MEASUREMENTS_FILE, POWERCAP);
           printf("[WARNING] Timeout: Function execution interrupted after %d seconds\n", TIME_OUT_LIMIT);
           return;
       }
    }
    fclose(fp);
}

int measureMemoryUsage(const char *command) {
   printf("[INFO] Measuring memory consumption of the program \"%s\"\n", command);
   char cmd[2048];
   // Redirect stderr to stdout, stdout to /dev/null, so the pipe captures time's output
   snprintf(cmd, sizeof(cmd),
            "/usr/bin/time -v %s 2>&1 1>/dev/null | grep 'Maximum resident set size' | "
            "sed 's/[^0-9]*\\([0-9]*\\).*/\\1/'",
            command);

   FILE *fp = popen(cmd, "r");
   if (!fp) {
       fprintf(stderr, "Error running command\n");
       return -1;
   }

   char buf[128];
   if (fgets(buf, sizeof(buf), fp) == NULL) {
       pclose(fp);
       fprintf(stderr, "Failed to read memory usage\n");
       return -1;
   }

   pclose(fp);
   printf("[INFO] Finished measuring memory consumption of the program \"%s\": %d KB\n", command, atoi(buf));

   return atoi(buf);
}

double calibrateTemperature(int NUMBER_OF_SECONDS) {
   printf("[INFO] Sleeping %d seconds to calibrate the temperature...\n", NUMBER_OF_SECONDS);
   sleep(NUMBER_OF_SECONDS);

   // Run the sensors command and redirect output to a file
   int status = system("sensors > /tmp/cores_temperatures.txt");
   if (status != 0) {
       printf("[ERROR] Error executing command: sensors\n");
       return -1;
   }

   FILE *file = fopen("/tmp/cores_temperatures.txt", "r");
   if (file == NULL) {
       printf("[ERROR] Error opening file: /tmp/cores_temperatures.txt\n");
       return -1;
   }

   FILE *output_file = fopen("/tmp/cores_temperatures.txt", "a");
   if (output_file == NULL) {
       printf("[ERROR] Error opening file for writing: /tmp/cores_temperatures.txt\n");
       fclose(file);
       return -1;
   }

   char line[256];
   double temperatures[256];
   int temp_count = 0;
   regex_t regex;
   regmatch_t matches[2];
   regcomp(&regex, "Core [0-9]+: *\\+([0-9.]+)°C", REG_EXTENDED);

   while (fgets(line, sizeof(line), file)) {
       if (regexec(&regex, line, 2, matches, 0) == 0) {
           char temp_str[10];
           strncpy(temp_str, line + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
           temp_str[matches[1].rm_eo - matches[1].rm_so] = '\0';

           double temperature = atof(temp_str);
           temperatures[temp_count++] = temperature;

           // Extract core number
           char *core_number = strtok(line, ":");
           fprintf(output_file, "%s: %.1f°C\n", core_number, temperature);
       }
   }

   fclose(file);
   regfree(&regex);

   double mean_temp = 0.0;
   if (temp_count > 0) {
       for (int i = 0; i < temp_count; i++) {
           mean_temp += temperatures[i];
       }
       mean_temp /= temp_count;
   }

   for (int i = 0; i < temp_count; i++) {
       printf("Core %d temperature: %.1f°C\n", i, temperatures[i]);
   }

   fprintf(output_file, "Average temperature: %.1f°C\n\n", mean_temp);
   fclose(output_file);

   return mean_temp;
}

void calibratePowercap(int ntimes, int core, char* command, char* language, char* program, int min_powercap, int max_powercap, int POWERCAP, 
    int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE) {
   printf("[INFO] Powercap calibration: Calibrating power cap by running %s (%s) with different power limits...\n", program, language);

   // Open the output CSV file for writing
   FILE *outputFile = fopen("../Utils/powercap_calibration.csv", "w");
   if (outputFile == NULL) {
       perror("[ERROR] Powercap calibration: Error opening powercap_calibration.csv for writing");
       return;
   }
   fprintf(outputFile, "Language,Program,PowerLimit,Package,Core,GPU,DRAM,Time,Temperature,Memory\n");

   // Flag to indicate if the header has been written
   int headerWritten = 0;
   
   int memory = measureMemoryUsage(command);
   
   for (int power_cap = min_powercap; power_cap <= max_powercap; power_cap++) {
       POWERCAP = power_cap;
       printf("[INFO] Powercap calibration: Testing with POWERCAP = %d\n", power_cap);


       raplcap rc;
       if (initializeRapl(&rc, POWERCAP) != 0) {
           printf("[ERROR] Powercap calibration: Failed to initialize RAPL with power cap %d. Skipping...\n", power_cap);
           continue;
       }

       // Generate filename based on current power cap and program name
       char filename[256];
       snprintf(filename, sizeof(filename), "%s_%d.csv", program, power_cap);
       
       // Run the testing and create the individual CSV
       runTesting(ntimes, core, command, language, program, filename, POWERCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, 0);

       // Read the contents of the individual CSV and append to the output file
       FILE *inputFile = fopen(filename, "r");
       if (inputFile == NULL) {
           perror("[ERROR] Powercap calibration: Error opening individual CSV file for reading");
           raplcap_destroy(&rc);
           continue;
       }

       char buffer[1024];
       while (fgets(buffer, sizeof(buffer), inputFile) != NULL) {
           fputs(buffer, outputFile);
       }
       
       
       fclose(inputFile);

       // Remove the individual CSV file after reading
       if (remove(filename) != 0) {
           perror("[ERROR] Powercap calibration: Error removing individual CSV file");
       }

       // Clean up power cap
       if (raplcap_destroy(&rc) != 0) {
           printf("[ERROR] Powercap calibration: Error destroying CAP after test with POWERCAP = %d\n", power_cap);
           perror("[ERROR] Powercap calibration: raplcap_destroy");
       }

       // Small delay to prevent overheating issues
       sleep(1);
   }
   
   fclose(outputFile); // Close the output file
   
   
   // Change to parent directory and run the Python script
   int ret = system("cd .. && python3 Utils/processPowercapCalibrationCSV.py Utils/powercap_calibration.csv");
   if (ret == -1 || WEXITSTATUS(ret) != 0) {
       fprintf(stderr, "[ERROR] Unable to process powercap calibration CSV\n");
   }
   else {
       printf("[INFO] Power cap calibration CSV processed successfully\n");
   }
}


int main(int argc, char **argv) {
   char command[500];
   char language[500] = "";
   char program[500] = "";
   int n_times = 1;
   int powercap = -1;
   int core = 0;
   int sleep_time = 0;
   float temperature_threshold;
   raplcap rc;
   
   int variance = 0;
   int time_out_limit = 0;



   // Check for --help command
   if (argc == 2 && strcmp(argv[1], "--help") == 0) {
       printf("Usage:\n");
       printf("  %s --temperature-calibration <seconds>\n", argv[0]);
       printf("      Calibrate the average CPU core temperature by sleeping for the specified number of seconds before measuring the temperature.\n\n");
       
       printf("  %s --powercap-calibration <min>-<max> <variance> <time_out_limit> <n_times> <secs_to_sleep> [compile_command] <run_command> <language> <program_name>\n", argv[0]);
       printf("      Calibrate the system's power cap by testing power limits within the specified range (e.g., 2-25 Watts) using a custom program.\n");
       printf("      The process runs multiple tests within the given power cap range and applies the provided temperature variance (in °C) and time_out_limit (in minutes) for each test.\n");
       printf("      For compiled languages: provide both compile_command and run_command.\n");
       printf("      For interpreted languages: provide only run_command (compile_command can be empty string \"\").\n");
       printf("      The n_times argument specifies how many times to run the program to gather data at each power cap setting.\n\n");
       
   
       printf("  %s <command> <language> <program> <n_times> <variance> <time_out_limit> <sleep_time> <powercap>\n", argv[0]);
       printf("      Run the specified command for the given program and language, repeating it <n_times> times.\n");
       printf("      Uses the provided variance (temperature) and time_out_limit (in minutes).\n");
       printf("      sleep_time specifies how long to idle before running the command (in seconds).\n");
       printf("      powercap specifies the power cap in watts (use -1 for no power cap, or any value >= 1).\n\n");
   
       printf("Arguments:\n");
       printf("  <command>         The command to execute (e.g., './program').\n");
       printf("  <language>        The programming language (e.g., 'C', 'Python').\n");
       printf("  <program>         The name of the program being tested.\n");
       printf("  <n_times>          Number of times to execute the command (must be > 0).\n");
       printf("  <variance>        Acceptable temperature variance (>= 0).\n");
       printf("  <time_out_limit>  Timeout limit in minutes (must be > 0).\n");
       printf("  <sleep_time>      Idle time before execution in seconds (>= 0).\n");
       printf("  <powercap>       Power cap in watts (-1 for no cap, or >= 1 for power cap).\n\n");
   
       printf("Examples:\n");
       printf("  %s --temperature-calibration 10\n", argv[0]);
       printf("  %s --powercap-calibration 2-25 5 120 30 7 \"gcc fibonacci.c -o fib\" \"./fib\" C fibonacci\n", argv[0]);
       printf("  %s --powercap-calibration 2-25 5 120 30 7 \"\" \"python3 script.py\" Python script\n", argv[0]);
       printf("  %s \"./my_program\" C my_program 5 3 2 10 15\n", argv[0]);
   
       return 0;
   }

   if (argc == 3 && strcmp(argv[1], "--temperature-calibration") == 0) {
       int sleepTime = atoi(argv[2]);
       calibrateTemperature(sleepTime);
       return 0;
   }

   if ((argc == 11 || argc == 10) && strcmp(argv[1], "--powercap-calibration") == 0) {
       int min_powercap, max_powercap, secs_to_sleep, n_times;
       char compile_command[500] = "";
       char run_command[500] = "";
       char language[500] = "";
       char program_name[500] = "";

       if (sscanf(argv[2], "%d-%d", &min_powercap, &max_powercap) != 2 || min_powercap < 0 || max_powercap < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid format. Use --powercap-calibration <min>-<max> (e.g., --powercap-calibration 2-25)\n");
           return 1;
       }

       if (sscanf(argv[3], "%d", &variance) != 1 || variance < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid variance. Must be a non-negative integer.\n");
           return 1;
       }

       if (sscanf(argv[4], "%d", &time_out_limit) != 1 || time_out_limit <= 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid time_out_limit. Must be a positive integer (in minutes).\n");
           return 1;
       }

       time_out_limit = time_out_limit * 60; // Convert to seconds

       if (sscanf(argv[5], "%d", &secs_to_sleep) != 1 || secs_to_sleep < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid secs_to_sleep. Must be a non-negative integer.\n");
           return 1;
       }

       if (sscanf(argv[6], "%d", &n_times) != 1 || n_times <= 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid n_times. Must be a positive integer.\n");
           return 1;
       }

       if (argc == 11) {
           // For compiled languages: compile_command run_command language program_name
           strcpy(compile_command, argv[7]);
           strcpy(run_command, argv[8]);
           strcpy(language, argv[9]);
           strcpy(program_name, argv[10]);
       } else if (argc == 10) {
           // For interpreted languages: run_command language program_name (no compile command)
           strcpy(run_command, argv[7]);
           strcpy(language, argv[8]);
           strcpy(program_name, argv[9]);
       }

       // Compile the program if compile command is provided and not empty
       if (strlen(compile_command) > 0) {
           printf("[INFO] Powercap calibration: Compiling program with command: %s\n", compile_command);
           if (system(compile_command) != 0) {
               fprintf(stderr, "[ERROR] Powercap calibration: Failed to compile program with command: %s\n", compile_command);
               return 1;
           }
       }

       if (access("/tmp/cores_temperatures.txt", F_OK) != 0) {
           printf("[WARNING] Powercap calibration: File /tmp/cores_temperatures.txt not found. Running temperature calibration...\n");
           calibrateTemperature(secs_to_sleep);
       } else {
           printf("[INFO] Powercap calibration: File /tmp/cores_temperatures.txt found. Skipping temperature calibration.\n");
       }
       temperature_threshold = getInitialTemperature();

       calibratePowercap(n_times, core, run_command, language, program_name, min_powercap, max_powercap, powercap, time_out_limit, temperature_threshold, variance);

       return 0;
   }

   if (argc != 9) {
       fprintf(stderr, "[ERROR] Incorrect number of arguments.\n");
       fprintf(stderr, "Usage: %s <command> <language> <program> <n_times> <variance> <time_out_limit> <sleep_time> <powercap>\n", argv[0]);
       fprintf(stderr, "Or run with --help for more information.\n");
       return -1;
   }

   strcpy(command, argv[1]);
   strcpy(language, argv[2]);
   strcpy(program, argv[3]);
   n_times = atoi(argv[4]);
   variance = atoi(argv[5]);
   time_out_limit = atoi(argv[6]); // time_out_limit in hours

   sleep_time = atoi(argv[7]);
   powercap = atoi(argv[8]);
   


   if (access("/tmp/cores_temperatures.txt", F_OK) != 0) {
       printf("[WARNING] File /tmp/cores_temperatures.txt not found. Running temperature calibration...\n");
       calibrateTemperature(sleep_time);
   } else {
       printf("[INFO] File /tmp/cores_temperatures.txt found. Skipping temperature calibration.\n");
   }

   temperature_threshold = getInitialTemperature();

   printf("\n[INFO]\n======= Execution Details ========\n");
   printf(" Command   : %s\n", command);
   printf(" Language  : %s\n", language);
   printf(" Program   : %s\n", program);
   printf(" N_times    : %d\n", n_times);
   printf(" Variance  : %d°C\n", variance);
   printf(" Timeout   : %d hours\n", time_out_limit);
   printf(" Sleep_time : %d seconds\n", sleep_time);
   if (powercap == -1) {
       printf(" PowerCap  : No power cap\n");
   } else {
       printf(" PowerCap  : %d watts\n", powercap);
   }
   printf(" Reference temperature : %.2f °C\n", temperature_threshold);
   printf("====================================\n\n");

   if (sleep_time < 0) {
       fprintf(stderr, "[ERROR] sleep_time must be greater than or equal to 0.\n");
       return -1;
   }

   if (n_times <= 0) {
       fprintf(stderr, "[ERROR] n_times must be greater than 0.\n");
       return -1;
   }
   if (variance < 0) {
       fprintf(stderr, "[ERROR] variance must be greater than or equal to 0.\n");
       return -1;
   }
   if (time_out_limit <= 0) {
       fprintf(stderr, "[ERROR] time_out_limit must be greater than 0.\n");
       return -1;
   }
   
   if (powercap < -1 || powercap == 0) {
       fprintf(stderr, "[ERROR] powercap must be -1 (no power cap) or >= 1 (power cap in watts).\n");
       return -1;
   }
   
   time_out_limit = time_out_limit * 3600; // time_out_limit in seconds

   fflush(stdout);

   int memory = measureMemoryUsage(command);

   if (powercap != -1) {
       if (initializeRapl(&rc, powercap))
           return -1;
   }
   
   // Command to execute for measuring metrics while the OS is idle
   char sleep_command[100];
   snprintf(sleep_command, sizeof(sleep_command), "sleep %d", sleep_time);

   printf("\n\n BEGINNING of IDLE EXECUTION: \n");
   runTesting(n_times, core, sleep_command, language, program, "measurements.csv", powercap, time_out_limit, temperature_threshold, variance, memory, 1);
   printf("\n\n END of IDLE EXECUTION: \n");

   printf("\n\n BEGINNING of PARAMETRIZED PROGRAM: \n");
   runTesting(n_times, core, command, language, program, "measurements.csv", powercap, time_out_limit, temperature_threshold, variance, memory, 0);
   printf("\n\n END of PARAMETRIZED PROGRAM: \n");

   if (powercap != -1) {
       if (raplcap_destroy(&rc)) {
           printf("[ERROR] Error destroying CAP\n");
           perror("[ERROR] raplcap_destroy");
       } else {
           printf("[INFO] Successfully destroyed CAP\n");
       }
   }

   return 0;
}
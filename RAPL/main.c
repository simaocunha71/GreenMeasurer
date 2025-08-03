/*
 * GreenMeasurer - Energy Consumption Measurement Framework
 * 
 * A comprehensive tool for measuring energy consumption of programs
 * across multiple programming languages using RAPL technology.
 * 
 * Authors: Simão Cunha, João Saraiva
 * University of Minho
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

void performMeasurements(const char *command, const char *language, const char *program, int ntimes, int core, const char* MEASUREMENTS_FILE, int WHATTSCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle);


// Structure to hold function return value and timeout flag
struct ThreadData {
    int returnValue;
};

// Global structure to communicate between threads
struct ThreadData threadData;


int initializeRapl(raplcap *rc, int WHATTSCAP){
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
    rl_long.watts = WHATTSCAP;
    rl_long.seconds = 0;
    rl_short.watts = WHATTSCAP;
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
       fprintf(stderr, "[ERROR] Falha ao executar o comando\n");
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

void writeErrorMessage(const char *language, const char *program, const char *errorMsg, const char* MEASUREMENTS_FILE, int WHATTSCAP){
    FILE *fp;
    fp = fopen(MEASUREMENTS_FILE, "w");
    if (fp == NULL) {
        perror("[ERROR] Error opening measurements file");
        exit(-1);
    }

    fprintf(fp, "%s, %s, %d, %s, %s, %s, %s, %s, %s, %s\n", language, program, WHATTSCAP,
                        errorMsg,errorMsg,errorMsg,errorMsg,errorMsg,errorMsg,errorMsg);
    fclose(fp);
}

void runTesting(int ntimes, int core, const char *command, const char *language, const char *program, const char* MEASUREMENTS_FILE, int WHATTSCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle) {    
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
        writeErrorMessage(language,program,"TimeOut", MEASUREMENTS_FILE, WHATTSCAP);
        printf("[WARNING] Timeout: Function execution interrupted after %d seconds\n", TIME_OUT_LIMIT);
    } else {
        // Function executed within the specified timeout
        if(threadData.returnValue==1){ //Program works
            performMeasurements(command,language,program,ntimes,core, MEASUREMENTS_FILE, WHATTSCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, idle);
        }
        else{
            writeErrorMessage(language,program,"ProgramError", MEASUREMENTS_FILE, WHATTSCAP);
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
void performMeasurements(const char *command, const char *language, const char *program, int ntimes, int core, const char* MEASUREMENTS_FILE, int WHATTSCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE, int memory, int idle) {
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
           fprintf(fp, "%s, idle_%s, %d, ", language, program, WHATTSCAP);
       }
       else {
           fprintf(fp, "%s, %s, %d, ", language, program, WHATTSCAP);
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
           writeErrorMessage(language,program,"TimeOut", MEASUREMENTS_FILE, WHATTSCAP);
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

void calibratePowercap(int ntimes, int core, char* command, char* language, char* program, int min_powercap, int max_powercap, int WHATTSCAP, int TIME_OUT_LIMIT, float TEMPERATURETHRESHOLD, int VARIANCE) {
   printf("[INFO] Powercap calibration: Calibrating power cap by running fib(20) with different power limits...\n");

   // Open the output CSV file for writing
   FILE *outputFile = fopen("powercap_calibration.csv", "w");
   if (outputFile == NULL) {
       perror("[ERROR] Powercap calibration: Error opening powercap_calibration.csv for writing");
       return;
   }
   fprintf(outputFile, "Language,Program,PowerLimit,Package,Core,GPU,DRAM,Time,Temperature,Memory\n");

   // Flag to indicate if the header has been written
   int headerWritten = 0;
   
   int memory = measureMemoryUsage(command);
   
   for (int power_cap = min_powercap; power_cap <= max_powercap; power_cap++) {
       WHATTSCAP = power_cap;
       printf("[INFO] Powercap calibration: Testing with WHATTSCAP = %d\n", power_cap);


       raplcap rc;
       if (initializeRapl(&rc, WHATTSCAP) != 0) {
           printf("[ERROR] Powercap calibration: Failed to initialize RAPL with power cap %d. Skipping...\n", power_cap);
           continue;
       }

       // Generate filename based on current power cap
       char filename[256];
       snprintf(filename, sizeof(filename), "fib_%d.csv", power_cap);
       
       // Run the testing and create the individual CSV
       runTesting(ntimes, core, command, language, program, filename, WHATTSCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, 0);

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
           printf("[ERROR] Powercap calibration: Error destroying CAP after test with WHATTSCAP = %d\n", power_cap);
           perror("[ERROR] Powercap calibration: raplcap_destroy");
       }

       // Small delay to prevent overheating issues
       sleep(1);
   }
   
   fclose(outputFile); // Close the output file
   

   int ret = system("/usr/bin/python3 ../Utils/processPowercapCalibrationCSV.py ../Utils/powercap_calibration.csv");
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
   int ntimes = 1;
   int WHATTSCAP = 11;
   int core = 0;
   int sleep_time = 0;
   float TEMPERATURETHRESHOLD;
   raplcap rc;
   
   int VARIANCE = 0;
   int TIME_OUT_LIMIT = 0;



   // Check for --help command
   if (argc == 2 && strcmp(argv[1], "--help") == 0) {
       printf("Usage:\n");
       printf("  %s --temperature-calibration <seconds>\n", argv[0]);
       printf("      Calibrate the average CPU core temperature by sleeping for the specified number of seconds before measuring the temperature.\n\n");
       
       printf("  %s --powercap-calibration <min>-<max> <VARIANCE> <TIME_OUT_LIMIT> <N_TIMES>\n", argv[0]);
       printf("      Calibrate the system's power cap by testing power limits within the specified range (e.g., 2-25 Watts) using the Fibonacci algorithm in C (executes fibonacci(20)).\n");
       printf("      The process runs multiple tests within the given power cap range and applies the provided temperature VARIANCE (in °C) and TIME_OUT_LIMIT (in minutes) for each test.\n");
       printf("      The N_TIMES argument specifies how many times to run the Fibonacci calculation to gather data at each power cap setting.\n\n");
       
   
       printf("  %s <command> <language> <program> <ntimes> <VARIANCE> <TIME_OUT_LIMIT> <sleep_time>\n", argv[0]);
       printf("      Run the specified command for the given program and language, repeating it <ntimes> times.\n");
       printf("      Uses the provided VARIANCE (temperature) and TIME_OUT_LIMIT (in minutes).\n");
       printf("      sleep_time specifies how long to idle before running the command (in seconds).\n\n");
   
       printf("Arguments:\n");
       printf("  <command>         The command to execute (e.g., './program').\n");
       printf("  <language>        The programming language (e.g., 'C', 'Python').\n");
       printf("  <program>         The name of the program being tested.\n");
       printf("  <ntimes>          Number of times to execute the command (must be > 0).\n");
       printf("  <VARIANCE>        Acceptable temperature variance (>= 0).\n");
       printf("  <TIME_OUT_LIMIT>  Timeout limit in minutes (must be > 0).\n");
       printf("  <sleep_time>      Idle time before execution in seconds (>= 0).\n\n");
   
       printf("Examples:\n");
       printf("  %s --temperature-calibration 10\n", argv[0]);
       printf("  %s --powercap-calibration 2-25 5 120 30 7\n", argv[0]);
       printf("  %s \"./my_program\" C my_program 5 3 2 10\n", argv[0]);
   
       return 0;
   }

   if (argc == 3 && strcmp(argv[1], "--temperature-calibration") == 0) {
       int sleepTime = atoi(argv[2]);
       calibrateTemperature(sleepTime);
       return 0;
   }

   if (argc == 7 && strcmp(argv[1], "--powercap-calibration") == 0) {
       int min_powercap, max_powercap, SECS_TO_SLEEP, N_TIMES;

       if (sscanf(argv[2], "%d-%d", &min_powercap, &max_powercap) != 2 || min_powercap < 0 || max_powercap < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid format. Use --powercap-calibration <min>-<max> (e.g., --powercap-calibration 2-25)\n");
           return 1;
       }

       if (sscanf(argv[3], "%d", &VARIANCE) != 1 || VARIANCE < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid VARIANCE. Must be a non-negative integer.\n");
           return 1;
       }

       if (sscanf(argv[4], "%d", &TIME_OUT_LIMIT) != 1 || TIME_OUT_LIMIT <= 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid TIME_OUT_LIMIT. Must be a positive integer (in minutes).\n");
           return 1;
       }

       TIME_OUT_LIMIT = TIME_OUT_LIMIT * 60; // Convert to seconds

       if (sscanf(argv[5], "%d", &SECS_TO_SLEEP) != 1 || SECS_TO_SLEEP < 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid SECS_TO_SLEEP. Must be a non-negative integer.\n");
           return 1;
       }

       if (sscanf(argv[6], "%d", &N_TIMES) != 1 || N_TIMES <= 0) {
           fprintf(stderr, "[ERROR] Powercap calibration: Invalid N_TIMES. Must be a positive integer.\n");
           return 1;
       }

       if (chdir("../Utils") != 0) {
           perror("[ERROR] Powercap calibration: Failed to change directory to ../Utils");
           return 1;
       }

       if (system("gcc fibonacci.c -o fib") != 0) {
           perror("[ERROR] Powercap calibration: Failed to compile fibonacci.c");
           return 1;
       }

       if (access("/tmp/cores_temperatures.txt", F_OK) != 0) {
           printf("[WARNING] Powercap calibration: File /tmp/cores_temperatures.txt not found. Running temperature calibration...\n");
           calibrateTemperature(SECS_TO_SLEEP);
       } else {
           printf("[INFO] Powercap calibration: File /tmp/cores_temperatures.txt found. Skipping temperature calibration.\n");
       }
       TEMPERATURETHRESHOLD = getInitialTemperature();

       calibratePowercap(N_TIMES, core, "./fib", "C", "fib", min_powercap, max_powercap, WHATTSCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE);

       if (chdir("../RAPL") != 0) {
           perror("[ERROR] Powercap calibration: Failed to change directory back to ../RAPL");
           return 1;
       }
       return 0;
   }

   if (argc != 8) {
       fprintf(stderr, "[ERROR] Incorrect number of arguments.\n");
       fprintf(stderr, "Usage: %s <command> <language> <program> <ntimes> <VARIANCE> <TIME_OUT_LIMIT> <sleep_time>\n", argv[0]);
       fprintf(stderr, "Or run with --help for more information.\n");
       return -1;
   }

   strcpy(command, argv[1]);
   strcpy(language, argv[2]);
   strcpy(program, argv[3]);
   ntimes = atoi(argv[4]);
   VARIANCE = atoi(argv[5]);
   TIME_OUT_LIMIT = atoi(argv[6]); //TIME_OUT_LIMIT in hours

   sleep_time = atoi(argv[7]);
   
   TIME_OUT_LIMIT = TIME_OUT_LIMIT * 3600; //TIME_OUT_LIMIT in seconds

   if (access("/tmp/cores_temperatures.txt", F_OK) != 0) {
       printf("[WARNING] File /tmp/cores_temperatures.txt not found. Running temperature calibration...\n");
       calibrateTemperature(sleep_time);
   } else {
       printf("[INFO] File /tmp/cores_temperatures.txt found. Skipping temperature calibration.\n");
   }

   TEMPERATURETHRESHOLD = getInitialTemperature();

   printf("\n[INFO]\n======= Execution Details ========\n");
   printf(" Command   : %s\n", command);
   printf(" Language  : %s\n", language);
   printf(" Program   : %s\n", program);
   printf(" NTimes    : %d\n", ntimes);
   printf(" Variance  : %d°C\n", VARIANCE);
   printf(" Timeout   : %d minutes\n", TIME_OUT_LIMIT);
   printf(" SleepTime : %d seconds\n", sleep_time);
   printf(" Reference temperature : %.2f °C\n", TEMPERATURETHRESHOLD);
   printf("====================================\n\n");

   if (sleep_time < 0) {
       fprintf(stderr, "[ERROR] sleep_time must be greater than or equal to 0.\n");
       return -1;
   }

   if (ntimes <= 0) {
       fprintf(stderr, "[ERROR] ntimes must be greater than 0.\n");
       return -1;
   }
   if (VARIANCE < 0) {
       fprintf(stderr, "[ERROR] VARIANCE must be greater than or equal to 0.\n");
       return -1;
   }
   if (TIME_OUT_LIMIT <= 0) {
       fprintf(stderr, "[ERROR] TIME_OUT_LIMIT must be greater than 0.\n");
       return -1;
   }
   

   fflush(stdout);

   int memory = measureMemoryUsage(command);

   if (WHATTSCAP != -1) {
       if (initializeRapl(&rc, WHATTSCAP))
           return -1;
   }
   
   // Command to execute for measuring metrics while the OS is idle
   char sleep_command[100];
   snprintf(sleep_command, sizeof(sleep_command), "sleep %d", sleep_time);

   printf("\n\n BEGINNING of IDLE EXECUTION: \n");
   runTesting(ntimes, core, sleep_command, language, program, "measurements.csv", WHATTSCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, 1);
   printf("\n\n END of IDLE EXECUTION: \n");

   printf("\n\n BEGINNING of PARAMETRIZED PROGRAM: \n");
   runTesting(ntimes, core, command, language, program, "measurements.csv", WHATTSCAP, TIME_OUT_LIMIT, TEMPERATURETHRESHOLD, VARIANCE, memory, 0);
   printf("\n\n END of PARAMETRIZED PROGRAM: \n");

   if (WHATTSCAP != -1) {
       if (raplcap_destroy(&rc)) {
           printf("[ERROR] Error destroying CAP\n");
           perror("[ERROR] raplcap_destroy");
       } else {
           printf("[INFO] Successfully destroyed CAP\n");
       }
   }

   return 0;
}
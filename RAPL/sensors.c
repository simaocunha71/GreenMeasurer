#include "sensors.h"

float getTemperature() {
    char command[100];
    char output[200];
    float temperature, mean_temp = 0.0;
    int count = 0;

    // Run sensors command and capture output
    int status = system("sensors > temp.txt");
    if (status != 0) {
        printf("[ERROR] Error executing command: sensors > temp.txt\n");
        return -1;
    }
    FILE* temp_file = fopen("temp.txt", "r");
        
    // Parse temperature from output
    while(fgets(output, 200, temp_file)) {
        if(strstr(output, "Core")) {
            sscanf(output, "Core %*d: +%f°C", &temperature);
            mean_temp += temperature;
            count++;
        }
    }

    if (count > 0)
        mean_temp /= count;
        
    fclose(temp_file);
    remove("temp.txt");
    printf("\n[INFO] Current average temperature in all cores is %.10f°C\n", mean_temp);

    return mean_temp;
}

float getInitialTemperature() {
    FILE *file = fopen("/tmp/cores_temperatures.txt", "r");
    if (!file) {
        perror("[ERROR] Error opening file /tmp/cores_temperatures.txt");
        return -1;
    }

    char line[256];
    regex_t regex;
    regmatch_t matches[2];
    const char *pattern = "Average temperature: ([0-9]+\\.[0-9]+)°C";

    // Compile the regular expression
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        fprintf(stderr, "[ERROR] Error compiling regex (getInitialTemperature())\n");
        fclose(file);
        return -1;
    }

    float result = -1.0;

    // Read lines from the file
    while (fgets(line, sizeof(line), file)) {
        if (regexec(&regex, line, 2, matches, 0) == 0) {
            // Extract the substring corresponding to the number
            char buffer[16];
            int start = matches[1].rm_so;
            int end = matches[1].rm_eo;
            snprintf(buffer, end - start + 1, "%s", line + start);
            result = atof(buffer);
            break;
        }
    }

    regfree(&regex);
    fclose(file);
    return result;
}
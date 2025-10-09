#include "sensors.h"

float getTemperature() {
    FILE *fp = popen("sensors", "r");
    if (!fp) {
        perror("[ERROR] Failed to run sensors command");
        return -1.0;
    }

    char line[256];
    float temperature, mean_temp = 0.0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Core")) {
            char *plus = strchr(line, '+');
            if (plus) {
                temperature = atof(plus + 1);
                mean_temp += temperature;
                count++;
            }
        }
    }

    int status = pclose(fp);
    if (status == -1) {
        perror("[ERROR] Error closing pipe");
        return -1.0;
    }

    if (count == 0) {
        fprintf(stderr, "[ERROR] No core temperatures found.\n");
        return -1.0;
    }

    mean_temp /= count;
    printf("[INFO] Average CPU temperature: %.2f°C\n", mean_temp);

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
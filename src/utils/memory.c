#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/energy-backend.h"

int platform_measure_memory(const char *command) {
    if (!command || strlen(command) == 0) return -1;

    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
            "/usr/bin/time -v %s 2>&1 1>/dev/null | grep 'Maximum resident set size' | "
            "sed 's/[^0-9]*\\([0-9]*\\).*/\\1/'",
            command);

    if (n < 0 || n >= (int)sizeof(cmd)) {
        fprintf(stderr, "[ERROR] Command buffer overflow in platform_measure_memory\n");
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("[ERROR] Failed to run /usr/bin/time for memory measurement");
        return -1;
    }

    char buf[128] = "";
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    return atoi(buf);
}
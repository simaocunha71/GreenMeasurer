#include <stdio.h>
#include "system_uptime.h"

double get_system_uptime_seconds(void) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) return -1.0;

    double uptime;
    int read_ok = fscanf(file, "%lf", &uptime) == 1;
    int close_status = fclose(file);
    return read_ok && close_status == 0 ? uptime : -1.0;
}

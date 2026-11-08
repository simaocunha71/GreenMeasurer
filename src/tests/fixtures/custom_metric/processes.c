#include "processes.h"

#include <stdio.h>

int get_number_of_running_processes(void) {
    FILE *file = fopen("/proc/loadavg", "r");
    if (!file) return -1;
    double one, five, fifteen;
    unsigned running, total;
    int fields = fscanf(file, "%lf %lf %lf %u/%u", &one, &five, &fifteen, &running, &total);
    if (fclose(file) != 0 || fields != 5) return -1;
    return (int)running;
}

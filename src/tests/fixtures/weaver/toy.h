#ifndef TOY_H
#define TOY_H

int toy_setup(void);
void toy_teardown(void);
unsigned long toy_energy(void);
float toy_temperature(void);
int toy_processes(void);

double toy_thermal(void);
double toy_calibrate_thermal(unsigned seconds);
float toy_calibrate_temperature(unsigned seconds);
int toy_cap_set(int watts);
int toy_cap_restore(void);
int toy_calibrate(int ntimes, int core, const char *command,
                  const char *program, const char *csv, const char *script, int min_powercap,
                  int max_powercap, int timeout, float temperature, int variance, int memory);

#endif

#ifndef GREENMEASURER_CPU_BACKEND_TEMPLATE_H
#define GREENMEASURER_CPU_BACKEND_TEMPLATE_H

#include "energy-backend.h"

static int backend_detect(void);
static int backend_init(int device);
static void backend_cleanup(void);
static int backend_sample_start(int device);
static int backend_sample_stop(int device);
static double backend_energy(int device, pid_t pid);
static double backend_temperature(int device, pid_t pid);

#endif

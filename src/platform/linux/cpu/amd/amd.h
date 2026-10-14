#ifndef AMD_H
#define AMD_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#include "energy-backend.h"

/* AMD MSRs (Zen Family 17h+ from turbostat) */
#define AMD_MSR_RAPL_PWR_UNIT       0xC0010299
#define AMD_MSR_CORE_ENERGY_STAT    0xC001029A
#define AMD_MSR_PKG_ENERGY_STAT     0xC001029B

/* 32-bit counter wrap-around delta macro */
#define DELTA_WRAP32(new, old) \
    ((((unsigned long long)(new) << 32) - ((unsigned long long)(old) << 32)) >> 32)

/* Exported AMD Backend */
extern EnergyBackend amd_backend;

#endif /* AMD_H */
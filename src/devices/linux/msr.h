#ifndef GM_DEVICES_MSR_H
#define GM_DEVICES_MSR_H

/* Shared access to the Intel RAPL energy MSRs (/dev/cpu/<core>/msr). */

#include <stdint.h>

#define MSR_RAPL_POWER_UNIT         0x606

#define MSR_PKG_RAPL_POWER_LIMIT    0x610
#define MSR_PKG_ENERGY_STATUS       0x611
#define MSR_PKG_PERF_STATUS         0x613
#define MSR_PKG_POWER_INFO          0x614

#define MSR_PP0_POWER_LIMIT         0x638
#define MSR_PP0_ENERGY_STATUS       0x639
#define MSR_PP0_POLICY              0x63A
#define MSR_PP0_PERF_STATUS         0x63B

#define MSR_PP1_ENERGY_STATUS       0x641

#define MSR_DRAM_POWER_LIMIT        0x618
#define MSR_DRAM_ENERGY_STATUS      0x619
#define MSR_DRAM_PERF_STATUS        0x61B
#define MSR_DRAM_POWER_INFO         0x61C

#define POWER_UNIT_OFFSET           0
#define POWER_UNIT_MASK             0x0F
#define ENERGY_UNIT_OFFSET          0x08
#define ENERGY_UNIT_MASK            0x1F00
#define TIME_UNIT_OFFSET            0x10
#define TIME_UNIT_MASK              0xF000

typedef struct {
    int fd;
    double energy_unit;   /* joules per counter tick */
} RaplMsr;

/* One energy domain. The hardware counter is 32 bits wide; ticks accumulates its deltas. */
typedef struct {
    uint32_t reg;
    uint32_t last;
    uint64_t ticks;
    int primed;
    int seen_nonzero;
} RaplDomain;

#define RAPL_DOMAIN(register_number) {.reg = (register_number)}

/* Opens the MSR device and reads the energy unit. Returns 0 on success. */
int rapl_msr_open(RaplMsr *msr, int core);
void rapl_msr_close(RaplMsr *msr);

/* Cumulative energy of the domain in joules, or -1 when the domain is unreadable or absent. */
double rapl_msr_energy(const RaplMsr *msr, RaplDomain *domain);

#endif

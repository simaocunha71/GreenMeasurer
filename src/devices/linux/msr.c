#include "msr.h"
#include "energy-counter.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static int read_msr(int fd, uint32_t reg, uint64_t *value) {
    return fd >= 0 && pread(fd, value, sizeof(*value), (off_t)reg) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int rapl_msr_open(RaplMsr *msr, int core) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", core);
    msr->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (msr->fd < 0) {
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "[WARNING] Permission denied accessing %s (run with sudo for Intel MSR metrics).\n", path);
        else if (errno == ENXIO)
            fprintf(stderr, "[WARNING] rdmsr: No CPU %d\n", core);
        else if (errno == EIO)
            fprintf(stderr, "[WARNING] rdmsr: CPU %d doesn't support MSRs\n", core);
        return -1;
    }

    uint64_t units;
    /* A VM can expose the MSR device but return zeros for every register. */
    if (read_msr(msr->fd, MSR_RAPL_POWER_UNIT, &units) != 0 || units == 0) {
        rapl_msr_close(msr);
        return -1;
    }
    msr->energy_unit = pow(0.5, (double)((units >> 8) & 0x1f));
    return 0;
}

void rapl_msr_close(RaplMsr *msr) {
    if (msr->fd >= 0) close(msr->fd);
    msr->fd = -1;
}

double rapl_msr_energy(const RaplMsr *msr, RaplDomain *domain) {
    uint64_t raw;
    if (read_msr(msr->fd, domain->reg, &raw) != 0) return -1.0;
    uint32_t now = (uint32_t)raw;
    /* A real counter never stays at 0; a stub or unsupported domain does. */
    if (now != 0) domain->seen_nonzero = 1;
    if (!domain->seen_nonzero) return -1.0;
    if (domain->primed) domain->ticks += energy_counter_delta32(now, domain->last);
    domain->primed = 1;
    domain->last = now;
    return (double)domain->ticks * msr->energy_unit;
}

#ifndef ENERGY_COUNTER_H
#define ENERGY_COUNTER_H

#include <stdint.h>

static inline uint32_t energy_counter_delta32(uint32_t after, uint32_t before) {
    return after >= before ? after - before : (UINT32_MAX - before) + 1u + after;
}

#endif

#include "../include/energy-counter.h"
#include <assert.h>

int main(void) {
    assert(energy_counter_delta32(20, 10) == 10);
    assert(energy_counter_delta32(10, 10) == 0);
    assert(energy_counter_delta32(0, UINT32_MAX) == 1);
    assert(energy_counter_delta32(3, UINT32_MAX - 2) == 6);
    return 0;
}

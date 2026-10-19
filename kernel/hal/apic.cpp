#include "kernel/hal/hal.h"

namespace apic {
static u64 tick_cycles = 1000000;
void init(){}
void calibrate(){}
void arm(u64){}
u64 period(){ return tick_cycles; }
}

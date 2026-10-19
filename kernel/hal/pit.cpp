#include "kernel/hal/hal.h"

namespace pit {
void init_hz(u32 hz){
    u32 div = 1193182u / hz;
    if (div > 65535) div = 65535;
    if (div < 1) div = 1;
    hal::outb(0x43, 0x36);
    hal::outb(0x40, (u8)(div & 0xFF));
    hal::outb(0x40, (u8)((div >> 8) & 0xFF));
}
}

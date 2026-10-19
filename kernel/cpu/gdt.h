// kernel/cpu/gdt.h — Refactor Session 11b (partial)
// Minimal-Header fuer gdt::-Symbole, die aus anderen Modulen gebraucht
// werden. Die Definition lebt noch in nexus.cpp (bis S11d).
#ifndef KERNEL_CPU_GDT_H
#define KERNEL_CPU_GDT_H

#include "kernel/hal/hal.h"

namespace gdt {

void init();
void  tss_set_rsp0(u64 rsp0);
u64   tss_get_rsp0();

} // namespace gdt

#endif

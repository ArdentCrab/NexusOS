// kernel/syscall/per_cpu.h — Refactor Session 9
#ifndef KERNEL_SYSCALL_PER_CPU_H
#define KERNEL_SYSCALL_PER_CPU_H

#include "kernel/hal/hal.h"

namespace task { struct TCB; }

struct PerCPU {
    PerCPU* self; u32 cpu_id; u32 apic_id;
    task::TCB* current; task::TCB* idle;
    u64 ticks; u64 switches;
    u64 kernel_rsp;    // offset 48: Syscall-Einstiegs-Stack
    u64 user_rsp;      // offset 56: gesicherter User-RSP waehrend syscall
};

static inline PerCPU* this_cpu(){
    PerCPU* p; __asm__ volatile("movq %%gs:0,%0":"=r"(p)); return p;
}

static void set_gs_base(void* p){ hal::wrmsr(0xC0000101,(u64)p); }

extern PerCPU g_cpu;

#endif

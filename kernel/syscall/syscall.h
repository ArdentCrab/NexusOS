// kernel/syscall/syscall.h — Refactor Session 9
#ifndef KERNEL_SYSCALL_SYSCALL_H
#define KERNEL_SYSCALL_SYSCALL_H

#include "kernel/hal/hal.h"

namespace syscall_msr {
void init();
}

extern "C" void syscall_entry();
extern "C" u64  syscall_dispatch(u64* regs);

#endif

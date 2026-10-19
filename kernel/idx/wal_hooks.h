// kernel/idx/wal_hooks.h — Refactor Session 4
#ifndef KERNEL_IDX_WAL_HOOKS_H
#define KERNEL_IDX_WAL_HOOKS_H

#include "kernel/hal/hal.h"

extern "C" void nexus_wal_put(u64 key, u32 type, u64 a, u64 b);
extern "C" void nexus_wal_remove(u64 key);
extern "C" bool nexus_wal_in_recovery();

#endif
